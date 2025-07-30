import networkx as nx
from collections import deque
import walker
from sentence_transformers import SentenceTransformer, util
import copy
import random
from operator import attrgetter
import numpy as np
from scipy.spatial.distance import cosine

node_similarity_threshold = 0.8
relation_similarity_threshold = 0.4

encoder = SentenceTransformer('all-MiniLM-L6-v2') # Model to create embeddings

def get_similarity_score(lhs, rhs):
    return util.cos_sim(lhs, rhs)

def node_similarity_function(node, embedding):
    return util.cos_sim(node['embedding'], embedding) > node_similarity_threshold

def edge_similarity_function(edge, embedding):
    return util.cos_sim(edge['embedding'], embedding) > relation_similarity_threshold
    
class DFA_State:
    def __init__(self, idx=None, name=None):
        self.idx = idx
        self.name = name
        self.name_embedding = encoder.encode(name) if name else None
        
    def __hash__(self) -> int:
        return self.idx
    
    def __eq__(self, other) -> bool:
        if isinstance(other, DFA_State):
            return self.idx == other.idx
        return False
        
    def __repr__(self) -> str:
        return f"State with name {self.name}"


class DFA_Edge:
    def __init__(self, relation=None):
        self.relation = relation
        self.relation_embedding = encoder.encode(relation) if relation else None
    
    def __hash__(self) -> int:
        return hash(self.relation)
    
    def __eq__(self, other) -> bool:
        if isinstance(other, DFA_Edge):
            return self.relation == other.relation
        return False
        
    def __repr__(self) -> str:
        return f"Edge with relation {self.relation}"

class DFA:
    def __init__(self):
        self.transitions = {}
        self.start_state = None
        self.current_state = None
        self.accept_states = set()
        self.str = ""

    def add_state(self, state, is_start=False, is_accept=False):
        if state not in self.transitions:
            self.transitions[state] = {}
        if is_start:
            self.start_state = state
        if is_accept:
            self.accept_states.add(state)

    def add_transition(self, from_state, to_state, edge):
        if from_state not in self.transitions:
            self.transitions[from_state] = {}
        self.transitions[from_state][edge] = to_state

    def process_pattern(self, pattern: list, start_name="init"):
        self.str = ", ".join(pattern)
        self.current_state = DFA_State(0, start_name)
        self.add_state(self.current_state, is_start=True)
        current_state = self.current_state

        for idx, token in enumerate(pattern):
            if '|' in token:
                options = token.split('|')
                for option in options:
                    next_state = DFA_State(idx + 1)
                    relation = DFA_Edge(option) 
                    self.add_state(next_state, is_accept=(idx+1 == len(pattern)))
                    self.add_transition(current_state, next_state, relation)
                current_state = next_state
            else:
                next_state = DFA_State(idx + 1)
                self.add_state(next_state, is_accept=(idx+1 == len(pattern)))
                _token = token.rstrip('+')
                relation = DFA_Edge(_token)
                self.add_transition(current_state, next_state, relation)
                current_state = next_state
                if token.endswith('+'):
                    # add edge connecting itself
                    self.add_transition(current_state, next_state, relation)

    def recognize(self, sequence):
        current_state = self.start_state

        for token in sequence:
            if current_state not in self.transitions or token not in self.transitions[current_state]:
                return False
            current_state = self.transitions[current_state][token]

        return current_state in self.accept_states
    
    def process_one_token(self, token):
        if token in self.transitions[self.current_state]:
            self.current_state = self.transitions[self.current_state][token]
            
    def __repr__(self) -> str:
        return self.str
    
        
        
class Path:
    def __init__(self, start_node, dfa: DFA=None, tuples=[]) -> None:
        # accumulated scores according to different DFA, hardcode its dimension to be 3
        self.start_node = start_node
        self.dfa = dfa
        # path tuples containing (e,r,e) => [(e,r,e), ...] 
        self.tuples = tuples
        self.validity = True
        self.visited_nodes = {self.start_node}
        self.score = 1

    def add_new_tuple(self, new_tuple):
        new_path = copy.deepcopy(self)
        new_path.tuples.append(new_tuple)
        _, relation, next_node = new_tuple
        rel_embedding = relation['embedding']
        for dfa_relation in new_path.dfa.transitions[new_path.dfa.current_state]:
            similarity_score = get_similarity_score(dfa_relation.relation_embedding, rel_embedding)
            if similarity_score > relation_similarity_threshold:
                new_path.dfa.process_one_token(dfa_relation)
                new_path.score = new_path.score * 0.4 + similarity_score * 0.6
                new_path.visited_nodes.add(next_node)
        return new_path
    
    def if_accept_rel_embedding(self, rel_embedding):
        for relation in self.dfa.transitions[self.dfa.current_state]:
            if get_similarity_score(relation.relation_embedding, rel_embedding) > relation_similarity_threshold:
                return True
        return False
        
    def if_valid(self) -> bool:
        return self.validity
    
    def is_accepted(self) -> bool:
        if self.if_valid():
            if self.dfa.current_state in self.dfa.accept_states:
                return True
        return False
    
    def __len__(self):
        if self.tuples:
            return len(self.tuples)
        else:
            return 0
            
    def __repr__(self) -> str:
        result = ""
        for i, tuple in enumerate(self.tuples):
            if i == 0:
                h, r, t = tuple
                result += f"{h} -> {r['relation']} -> {t}"
            else:
                _, r, t = tuple
                result += f"-> {r['relation']} -> {t}"
        return result.strip()
        

def build_graph(kb_file_path) -> nx.Graph:
    G = nx.Graph()
    with open(kb_file_path, 'r') as file:
        for line in file:
            parts = line.strip().split('|')
            if len(parts) == 3:
                h, r, t = parts
                if h not in G:
                    h_embedding = encoder.encode(h)
                    G.add_node(h, embedding=h_embedding)
                if t not in G:
                    t_embedding = encoder.encode(t)
                    G.add_node(t, embedding=t_embedding)
                relation= r.strip()
                relation_embedding = encoder.encode(relation)
                G.add_edge(h, t, relation=relation, embedding=relation_embedding)
    return G


def get_truth_paths(q_entity: list, a_entity: list, graph: nx.Graph) -> list:
    '''
    Get shortest paths connecting question and answer entities.
    '''
    # Select paths
    paths = []
    for h in q_entity:
        if h not in graph:
            continue
        for t in a_entity:
            if t not in graph:
                continue
            try:
                for p in nx.all_shortest_paths(graph, h, t):
                    paths.append(p)
            except:
                pass
    # Add relation to paths
    result_paths = []
    for p in paths:
        tmp = []
        for i in range(len(p)-1):
            u = p[i]
            v = p[i+1]
            tmp.append((u, graph[u][v]['relation'], v))
        result_paths.append(tmp)
    return result_paths
    
def get_simple_paths(q_entity: list, a_entity: list, graph: nx.Graph, hop=2) -> list:
    '''
    Get all simple paths connecting question and answer entities within given hop
    '''
    # Select paths
    paths = []
    for h in q_entity:
        if h not in graph:
            continue
        for t in a_entity:
            if t not in graph:
                continue
            try:
                for p in nx.all_simple_edge_paths(graph, h, t, cutoff=hop):
                    paths.append(p)
            except:
                pass
    # Add relation to paths
    result_paths = []
    for p in paths:
        result_paths.append([(e[0], graph[e[0]][e[1]]['relation'], e[1]) for e in p])
    return result_paths

def get_negative_paths(q_entity: list, a_entity: list, graph: nx.Graph, n_neg: int, hop=2) -> list:
    '''
    Get negative paths for question witin hop
    '''
    # sample paths
    start_nodes = []
    end_nodes = []
    node_idx = list(graph.nodes())
    for h in q_entity:
        if h in graph:
            start_nodes.append(node_idx.index(h))
    for t in a_entity:
        if t in graph:
            end_nodes.append(node_idx.index(t))
    paths = walker.random_walks(graph, n_walks=n_neg, walk_len=hop, start_nodes=start_nodes, verbose=False)
    # Add relation to paths
    result_paths = []
    for p in paths:
        tmp = []
        # remove paths that end with answer entity
        if p[-1] in end_nodes:
            continue
        for i in range(len(p)-1):
            u = node_idx[p[i]]
            v = node_idx[p[i+1]]
            tmp.append((u, graph[u][v]['relation'], v))
        result_paths.append(tmp)
    return result_paths

def get_random_paths(q_entity: list, graph: nx.Graph, n=3, hop=2) -> tuple [list, list]:
    '''
    Get negative paths for question witin hop
    '''
    # sample paths
    start_nodes = []
    node_idx = list(graph.nodes())
    for h in q_entity:
        if h in graph:
            start_nodes.append(node_idx.index(h))
    paths = walker.random_walks(graph, n_walks=n, walk_len=hop, start_nodes=start_nodes, verbose=False)
    # Add relation to paths
    result_paths = []
    rules = []
    for p in paths:
        tmp = []
        tmp_rule = []
        for i in range(len(p)-1):
            u = node_idx[p[i]]
            v = node_idx[p[i+1]]
            tmp.append((u, graph[u][v]['relation'], v))
            tmp_rule.append(graph[u][v]['relation'])
        result_paths.append(tmp)
        rules.append(tmp_rule)
    return result_paths, rules

def construct_combined_DFA(regs: list) -> list[DFA]:
    assert len(regs) <= 3, "we only support at most 3 RPQs at the time"
    dfas= []
    for reg in regs:
        dfa = DFA()
        dfa.process_pattern(reg)
        dfas.append(dfa)
    # print(f"dfas are {dfas[0]} || {dfas[1]} || {dfas[2]}.")
    return dfas

def utility_calculation(node):
    node_embedding = node['embedding']
    norm_embedding = node_embedding / np.linalg.norm(node_embedding)
    # Use a fixed reference vector for cosine similarity
    reference_vector = np.ones_like(node_embedding)
    reference_vector /= np.linalg.norm(reference_vector)
    # Calculate cosine similarity
    cosine_sim = 1 - cosine(norm_embedding, reference_vector)
    # Directly use the sum of normalized embedding as a basic utility indicator
    basic_score = np.sum(norm_embedding)
    # Calculate utility as a weighted sum
    utility = 0.7 * basic_score + 0.3 * cosine_sim
    return utility

def semantic_bfs(start_node, target_rule, graph: nx.Graph, threshold, max_hop=3):
    '''
    Semantic Regular Path Query
    '''
    result_paths = []
    dfa = DFA()
    dfa.process_pattern(target_rule)
    queue = deque([(start_node, Path(start_node, dfa))])
    max_len = 0
    while queue:
        # selectiivty shedding
        current_node, current_path = queue.popleft()
        # if accepted
        if current_path.is_accepted():
            result_paths.append(current_path)
        if not current_path.if_valid() or current_node not in graph or len(current_path) > max_hop:
            continue
        for neighbor in graph.neighbors(current_node):
            if neighbor in current_path.visited_nodes or utility_calculation(graph.nodes[neighbor]) < threshold:
                continue
            rel = graph[current_node][neighbor]
            rel_embedding  = rel['embedding']
            # current path considering if accepting current neighbor
            if current_path.if_accept_rel_embedding(rel_embedding):
                queue.append((neighbor, current_path.add_new_tuple((current_node, rel, neighbor))))
    return result_paths
